#include "TestEngineSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunAudioRuntimeTests(Tests);
	return Tests.Finish();
}
