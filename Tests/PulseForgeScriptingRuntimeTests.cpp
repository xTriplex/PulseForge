#include "TestEngineSupport.h"

void RunScriptRuntimeTests(TestRunner& Tests);
void RunPlayerSessionTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunScriptRuntimeTests(Tests);
	RunPlayerSessionTests(Tests);
	return Tests.Finish();
}
