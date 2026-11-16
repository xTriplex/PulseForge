#include "TestSupport.h"
#include "Core/Log.h"

void RunRendererPolicyTests(TestRunner& Tests);
void RunAssetCacheInventoryStatsTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunRendererPolicyTests(Tests);
	RunAssetCacheInventoryStatsTests(Tests);
	RunRendererContractTests(Tests);
	RunRendererTests(Tests);
	return Tests.Finish();
}
