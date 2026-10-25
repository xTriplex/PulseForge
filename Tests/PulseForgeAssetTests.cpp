#include "TestAssetSupport.h"

#include <string_view>

void RunAssetDependencyCatalogTests(TestRunner& Tests);
void RunAssetDependencyCorrectionTests(TestRunner& Tests);
void RunAssetDependencyPublicationTests(TestRunner& Tests);

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	TestRunner Tests;
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--dependency-catalog-only")
	{
		RunAssetDependencyCatalogTests(Tests);
		RunAssetDependencyCorrectionTests(Tests);
		RunAssetDependencyPublicationTests(Tests);
		return Tests.Finish();
	}
	RunAssetsTests(Tests);
	RunAssetRegistryTests(Tests);
	RunAssetOperationsTests(Tests);
	RunSceneAssetTests(Tests);
	RunAssetDependencyCatalogTests(Tests);
	RunAssetDependencyCorrectionTests(Tests);
	RunAssetDependencyPublicationTests(Tests);
	return Tests.Finish();
}
