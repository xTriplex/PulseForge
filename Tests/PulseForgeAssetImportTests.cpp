#include "TestEngineSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunAssetImportTests(Tests);
	return Tests.Finish();
}
