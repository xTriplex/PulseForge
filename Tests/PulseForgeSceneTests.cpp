#include "TestSceneSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunSceneTests(Tests);
	return Tests.Finish();
}
