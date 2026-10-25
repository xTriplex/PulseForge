#include "TestSupport.h"
#include "Core/Log.h"

void RunAssetPreparationServiceTests(TestRunner& Tests);
void RunAssetPreparationMetricsTests(TestRunner& Tests);
void RunDerivedDataArtifactStoreTests(TestRunner& Tests);
void RunDerivedAssetPreparationTests(TestRunner& Tests);
void RunEnvironmentLightingCacheTests(TestRunner& Tests);
void RunAssetUploadBudgetTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunAssetPreparationServiceTests(Tests);
	RunAssetPreparationMetricsTests(Tests);
	RunDerivedDataArtifactStoreTests(Tests);
	RunDerivedAssetPreparationTests(Tests);
	RunEnvironmentLightingCacheTests(Tests);
	RunAssetUploadBudgetTests(Tests);
	return Tests.Finish();
}
