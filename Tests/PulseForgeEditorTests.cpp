#include "TestEditorSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunEditorHistoryTests(Tests);
	RunEditorViewportTests(Tests);
	RunEditorTests(Tests);
	RunEditorStateTests(Tests);
	RunDocumentsTests(Tests);
	RunProjectRuntimeTests(Tests);
	return Tests.Finish();
}
