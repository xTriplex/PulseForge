#include "TestEngineSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunGltfMeshImportTests(Tests);
	RunImageAssetImportTests(Tests);
	return Tests.Finish();
}
