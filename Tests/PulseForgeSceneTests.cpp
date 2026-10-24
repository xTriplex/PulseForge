#include "TestSceneSupport.h"

void RunPropertyMetadataTests(TestRunner& Tests);
void RunScriptPropertyMetadataTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunSceneTests(Tests);
	RunPropertyMetadataTests(Tests);
	RunScriptPropertyMetadataTests(Tests);
	return Tests.Finish();
}
