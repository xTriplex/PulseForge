#include "TestEditorSupport.h"

#include <spdlog/spdlog.h>

void RunEditorDocumentHostTestEditorTests(TestRunner& Tests);
void RunEditorInspectorRegressionTests(TestRunner& Tests);
#if defined(_DEBUG) || defined(PF_DEBUG)
void RunEditorDebugAcceptanceCoordinatorTests(TestRunner& Tests);
#endif

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	PF_CHECK(Tests, spdlog::get("PULSEFORGE") != nullptr);
	PF_CHECK(Tests, spdlog::get("APP") != nullptr);
	RunEditorHistoryTests(Tests);
	RunEditorViewportTests(Tests);
	RunEditorViewportSceneRenderViewTests(Tests);
	RunEditorAssetPreviewStressHarnessTests(Tests);
	RunEditorDocumentHostTestEditorTests(Tests);
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
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
