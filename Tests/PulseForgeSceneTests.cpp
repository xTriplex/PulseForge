#include "TestSceneSupport.h"

void RunPropertyMetadataTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunSceneTests(Tests);
	RunPropertyMetadataTests(Tests);
	return Tests.Finish();
}
