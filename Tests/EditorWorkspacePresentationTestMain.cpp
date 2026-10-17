#include "TestEditorSupport.h"

void RunEditorWorkspacePresentationTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunEditorWorkspacePresentationTests(Tests);
	return Tests.Finish();
}
