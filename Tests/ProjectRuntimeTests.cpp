#include "TestEditorSupport.h"

void RunEditorMaterialProjectTests(TestRunner& Tests);
void RunEditorProjectLoadingTests(TestRunner& Tests);
void RunEditorAssetReferenceTests(TestRunner& Tests);
void RunScriptRuntimeTests(TestRunner& Tests);

void RunProjectRuntimeTests(TestRunner& Tests)
{
	RunEditorMaterialProjectTests(Tests);
	RunEditorProjectLoadingTests(Tests);
	RunEditorAssetReferenceTests(Tests);
	RunScriptRuntimeTests(Tests);
}
