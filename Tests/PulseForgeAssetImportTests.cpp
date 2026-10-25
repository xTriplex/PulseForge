#include "TestEngineSupport.h"

void RunAssetImportProvenanceTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunGltfMeshImportTests(Tests);
	RunImageAssetImportTests(Tests);
	RunAssetImportProvenanceTests(Tests);
	return Tests.Finish();
}
