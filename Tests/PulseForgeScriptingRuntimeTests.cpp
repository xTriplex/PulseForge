#include "TestEngineSupport.h"

void RunScriptRuntimeTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunScriptRuntimeTests(Tests);
	return Tests.Finish();
}
