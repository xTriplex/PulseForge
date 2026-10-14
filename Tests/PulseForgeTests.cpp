#include "TestEngineSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunCoreTests(Tests);
	RunSceneTests(Tests);
	RunAssetsTests(Tests);
	RunRendererTests(Tests);
	RunImportsTests(Tests);
	return Tests.Finish();
}
