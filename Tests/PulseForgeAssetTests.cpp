#include "TestAssetSupport.h"

#include <exception>
#include <iostream>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <stdlib.h>
#endif

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
	try
	{
	#if defined(_WIN32)
		// Keep test failures observable in unattended CI without changing machine-wide error reporting.
		(void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
		#if defined(_MSC_VER)
		(void)_set_error_mode(_OUT_TO_STDERR);
		#endif
	#endif

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
			return Tests.Finish();
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--asset-core-only")
		{
			RunAssetsTests(Tests);
			RunAssetRegistryTests(Tests);
			RunAssetOperationsTests(Tests);
			RunSceneAssetTests(Tests);
			return Tests.Finish();
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--asset-edit-only")
		{
			RunAssetEditTransactionTests(Tests);
			return Tests.Finish();
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--cook-only")
		{
			std::cerr << "ASSET_TEST_SUITE_BEGIN cook" << std::endl;
			RunAssetCookServiceTests(Tests);
			std::cerr << "ASSET_TEST_SUITE_END cook" << std::endl;
			return Tests.Finish();
		}
		if (ArgumentCount == 2 && std::string_view(Arguments[1]) == "--export-only")
		{
			std::cerr << "ASSET_TEST_SUITE_BEGIN build-profile" << std::endl;
			RunExportBuildProfileTests(Tests);
			std::cerr << "ASSET_TEST_SUITE_END build-profile" << std::endl;
			std::cerr << "ASSET_TEST_SUITE_BEGIN export" << std::endl;
			RunAssetExportServiceTests(Tests);
			std::cerr << "ASSET_TEST_SUITE_END export" << std::endl;
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
	catch (const std::exception& Error)
	{
		std::cerr << "ASSET_TEST_UNCAUGHT_EXCEPTION: " << Error.what() << '\n' << std::flush;
		return 2;
	}
	catch (...)
	{
		std::cerr << "ASSET_TEST_UNCAUGHT_EXCEPTION: non-standard exception\n" << std::flush;
		return 2;
	}
}
