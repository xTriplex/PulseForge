#include "TestAssetSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunAssetsTests(Tests);
	RunAssetRegistryTests(Tests);
	RunAssetOperationsTests(Tests);
	RunSceneAssetTests(Tests);
	return Tests.Finish();
}
