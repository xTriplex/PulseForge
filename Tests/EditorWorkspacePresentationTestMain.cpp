#include "TestEditorSupport.h"

void RunEditorWorkspacePresentationTests(TestRunner& Tests);
void RunEditorPerformanceTraceTests(TestRunner& Tests);
void RunEditorResourceInventoryTests(TestRunner& Tests);
void RunEditorResourceInventoryServiceTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunEditorWorkspacePresentationTests(Tests);
	RunEditorPerformanceTraceTests(Tests);
	RunEditorResourceInventoryTests(Tests);
	RunEditorResourceInventoryServiceTests(Tests);
	return Tests.Finish();
}
