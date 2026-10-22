#include "TestEditorSupport.h"

void RunEditorWorkspacePresentationTests(TestRunner& Tests);
void RunEditorPerformanceTraceTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunEditorWorkspacePresentationTests(Tests);
	RunEditorPerformanceTraceTests(Tests);
	return Tests.Finish();
}
