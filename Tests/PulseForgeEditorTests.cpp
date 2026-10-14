#include "TestEditorSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunEditorHistoryTests(Tests);
	RunEditorTests(Tests);
	RunDocumentsTests(Tests);
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
