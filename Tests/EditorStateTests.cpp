#include "TestEditorSupport.h"

void RunEditorPanelStateTests(TestRunner& Tests);
void RunEditorSceneStateTests(TestRunner& Tests);

void RunEditorStateTests(TestRunner& Tests)
{
	RunEditorPanelStateTests(Tests);
	RunEditorSceneStateTests(Tests);
}
