#include "TestEngineSupport.h"

void RunAssetImportProvenanceTests(TestRunner& Tests);
void RunGltfMeshImporterRegressionTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunGltfMeshImportTests(Tests);
	RunImageAssetImportTests(Tests);
	RunAssetImportProvenanceTests(Tests);
	RunGltfMeshImporterRegressionTests(Tests);
	return Tests.Finish();
}
