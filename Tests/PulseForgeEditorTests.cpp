#include "TestEditorSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunEditorHistoryTests(Tests);
	RunEditorViewportTests(Tests);
	RunEditorViewportSceneRenderViewTests(Tests);
	RunEditorAssetPreviewStressHarnessTests(Tests);
	RunEditorTests(Tests);
	RunEditorStateTests(Tests);
	RunDocumentsTests(Tests);
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
