#include "TestAssetSupport.h"

#include <string_view>

void RunAssetDependencyCatalogTests(TestRunner& Tests);
void RunAssetDependencyCorrectionTests(TestRunner& Tests);
void RunAssetDependencyPublicationTests(TestRunner& Tests);
void RunAssetEditTransactionTests(TestRunner& Tests);
void RunAssetCookServiceTests(TestRunner& Tests);
void RunExportBuildProfileTests(TestRunner& Tests);
void RunAssetExportServiceTests(TestRunner& Tests);
void SetAssetEditTestExecutablePath(const std::filesystem::path& Executable);
int RunAssetEditTransactionChildMode(int ArgumentCount, char** Arguments);

int main(int ArgumentCount, char** Arguments)
{
	PulseForge::Log::Init();
	const int ChildResult = RunAssetEditTransactionChildMode(ArgumentCount, Arguments);
	if (ChildResult >= 0) return ChildResult;
	if (ArgumentCount > 0) SetAssetEditTestExecutablePath(std::filesystem::u8path(Arguments[0]));
	TestRunner Tests;
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--dependency-catalog-only")
	{
		RunAssetDependencyCatalogTests(Tests);
		RunAssetDependencyCorrectionTests(Tests);
		RunAssetDependencyPublicationTests(Tests);
		RunAssetEditTransactionTests(Tests);
		return Tests.Finish();
	}
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--asset-edit-only")
	{
		RunAssetEditTransactionTests(Tests);
		return Tests.Finish();
	}
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--cook-only")
	{
		RunAssetCookServiceTests(Tests);
		RunExportBuildProfileTests(Tests);
		RunAssetExportServiceTests(Tests);
		return Tests.Finish();
	}
	if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--export-only")
	{
		RunExportBuildProfileTests(Tests);
		RunAssetExportServiceTests(Tests);
		return Tests.Finish();
	}
	RunAssetsTests(Tests);
	RunAssetRegistryTests(Tests);
	RunAssetOperationsTests(Tests);
	RunSceneAssetTests(Tests);
	RunAssetDependencyCatalogTests(Tests);
	RunAssetDependencyCorrectionTests(Tests);
	RunAssetDependencyPublicationTests(Tests);
	RunAssetEditTransactionTests(Tests);
	RunAssetCookServiceTests(Tests);
	RunExportBuildProfileTests(Tests);
	RunAssetExportServiceTests(Tests);
	return Tests.Finish();
}
