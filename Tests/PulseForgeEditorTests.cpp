#include "TestEditorSupport.h"

#include <spdlog/spdlog.h>

void RunEditorDocumentHostTestEditorTests(TestRunner& Tests);
void RunPrefabLinkedInstanceEditorTests(TestRunner& Tests);
void RunEditorBuildProfileControllerTests(TestRunner& Tests);
void RunEditorAssetEditSchedulingTests(TestRunner& Tests);
void RunEditorInspectorRegressionTests(TestRunner& Tests);
void RunEditorScriptAuthoringInspectorTests(TestRunner& Tests);
void RunEditorRuntimeUIDocumentEditorTests(TestRunner& Tests);
void RunEditorGraphDocumentEditorTests(TestRunner& Tests);
#if defined(_DEBUG) || defined(PF_DEBUG)
void RunEditorDebugAcceptanceCoordinatorTests(TestRunner& Tests);
#endif

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	TestRunner Tests;
	PF_CHECK(Tests, spdlog::get("PULSEFORGE") != nullptr);
	PF_CHECK(Tests, spdlog::get("APP") != nullptr);
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--runtime-ui-document")
	{
		RunEditorRuntimeUIDocumentEditorTests(Tests);
		return Tests.Finish();
	}
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--graph-document")
	{
		RunEditorGraphDocumentEditorTests(Tests);
		return Tests.Finish();
	}
	RunEditorHistoryTests(Tests);
	RunEditorViewportTests(Tests);
	RunEditorBuildProfileControllerTests(Tests);
	RunEditorViewportSceneRenderViewTests(Tests);
	RunEditorAssetPreviewStressHarnessTests(Tests);
	RunEditorAssetEditSchedulingTests(Tests);
	RunEditorDocumentHostTestEditorTests(Tests);
	RunPrefabLinkedInstanceEditorTests(Tests);
#if defined(_DEBUG) || defined(PF_DEBUG)
	RunEditorDebugAcceptanceCoordinatorTests(Tests);
#endif
	RunEditorTests(Tests);
	RunEditorStateTests(Tests);
	RunEditorDocumentManagerTests(Tests);
	RunEditorDocumentEditorRegistryTests(Tests);
	RunEditorSnapshotHistoryTests(Tests);
	RunEditorDocumentWorkspaceTests(Tests);
	RunEditorAssetImportOperationTests(Tests);
	RunEditorInspectorRegressionTests(Tests);
	RunEditorScriptAuthoringInspectorTests(Tests);
	RunEditorRuntimeUIDocumentEditorTests(Tests);
	RunEditorGraphDocumentEditorTests(Tests);
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
