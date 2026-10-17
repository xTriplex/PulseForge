#include "TestEditorSupport.h"

#include <spdlog/spdlog.h>

void RunEditorDocumentHostTestEditorTests(TestRunner& Tests);

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
	RunEditorTests(Tests);
	RunEditorStateTests(Tests);
	RunEditorDocumentManagerTests(Tests);
	RunEditorDocumentEditorRegistryTests(Tests);
	RunEditorSnapshotHistoryTests(Tests);
	RunEditorDocumentWorkspaceTests(Tests);
	RunEditorAssetImportOperationTests(Tests);
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
