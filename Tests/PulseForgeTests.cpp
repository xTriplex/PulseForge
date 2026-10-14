#include "TestSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunCoreTests(Tests);
	RunSceneTests(Tests);
	RunEditorHistoryTests(Tests);
	RunEditorTests(Tests);
	RunAssetsTests(Tests);
	RunRendererTests(Tests);
	RunDocumentsTests(Tests);
	RunProjectRuntimeTests(Tests);
	RunImportsTests(Tests);
	return Tests.Finish();
}
